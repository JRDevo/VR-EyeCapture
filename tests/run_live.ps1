<#
    run_live.ps1 -- live share (the OBS plugin's data path) end to end, no OBS, no headset: tests/layer_e2e.exe drives a
    game frame loop through the real OpenXR loader, the layer and tests/mock_runtime; tests/live_share_test.exe is the
    consumer, built from the plugin's own obs/live_consumer.cpp. Each case uses a private block name
    (XR_EYE_CAPTURE_TEST_LIVE_SHM), so a running OBS with the plugin or a real game is never touched.
      powershell -ExecutionPolicy Bypass -File tests\run_live.ps1 [-InstalledLayer] [-Only <regex>]
    -Only: only the consumer cases whose name matches (the no-consumer / idle-block cases are skipped then).
    Default: build\Release\eye_capture.dll as an explicit layer (registered copy disabled for these processes, nothing
    copied or registered; the run_e2e.ps1 -BuildLayer mechanism). -InstalledLayer: the registered copy instead.
    Cases (each pins its re-projection keys with XR_EYE_CAPTURE_CONFIG, so config.ini does not matter):
      raw: d3d11 / two, d3d11 / wide, d3d12 / two, d3d12 / array   the consumer asks for RAW eyes while the config would
                     re-project (symmetric_fov 1, head_smoothing 0.6): eye sizes + format, 60 / 30 fps throttle, both
                     eyes' pixels = the test pattern (bit-exact), a held claim is never overwritten; then the consumer
                     closes (heartbeat cleared) -> the layer logs "live share: stopped (consumer gone ...)" while the game
                     still runs, with its counters. d3d11 / two runs with layer_e2e --overlays and record_overlays 1: raw
                     stays raw (bit-exact pattern, no overlay line); its in-headset indicator shows the LIVE dot (the mock
                     counts our quad layer on frames); d3d11 / wide with indicator_live 0: no quad layer at all.
      d3d12 / two abandon   (raw) the consumer process ends WITHOUT closing: the layer must stop on the 2 s heartbeat timeout.
      smooth: d3d11 / two --jitter   "same as recording" with symmetric_fov 1 + head_smoothing 0.6 against the mock's head
                     motion and asymmetric fovs: the ring is the symmetric output size (ComputeSymmetricFov), reprojected
                     bits 3, the far marker at the same place in both halves, no black corner, throttle, held claim; then the
                     consumer switches to raw eyes -> a new generation with the raw eye size. Log: the re-projection lines,
                     the summary, the switch stop, a raw ring after it.
      sym: d3d12 / two   symmetric_fov 1, head_smoothing 0 on the plain mock (symmetric +-0.8 rad, identity pose): the pass
                     on a D3D12 game's raw ring, the pattern within 2 code values.
      config off: d3d11 / two   "same as recording" with symmetric_fov 0 + head_smoothing 0 (record_overlays 1, but the
                     game has no overlay layer): raw eyes, bit-exact, no pass.
      half: d3d11 / two raw + half, d3d12 / two sym + half   the consumer asks for Size Half: the ring is half the eye size
                     ((n + 1) / 2), bits + 4, the pattern within 2 code values (2 x 2 taps of the pass; raw = identity mapping).
      overlays: d3d12 / two --overlays, half   "same as recording" with record_overlays 1 on a live-only game: the first ring
                     has no overlays, the first overlay layer restarts it ("the game shows overlay layers") with bits 1 | 8 | 4;
                     the layers are in the live frame where run_e2e.ps1 finds them in a recording, the indicator is not.
      record + live: d3d11 / two --overlays --record 8   a recording (autorecord) and the live view at the same time, both
                     with symmetric_fov 1, head_smoothing 0.6, record_overlays 1: the live frames as in "overlays" (full
                     size; no restart: the recording's overlay collection told the live share before its first ring), the
                     MP4 per verify_mp4.ps1 (codec, size, fps, duration, st3d, audio; pixels: -SkipPixels, the overlays
                     cover pattern steps) + the overlay / indicator pixels in it, recorder dropped 0, both overlay summaries.
      Every consumer case: the "stopped" lines must report 0 game-thread share calls over 4 ms (no stall of the game) and
                     the last one share calls > 0. layer_e2e's E2E_ENDFRAME line (whole xrEndFrame: capture + recording + live
                     share + indicator + the mock) is printed; the record + live case requires 0 frames over 4 ms after its
                     first second (ring / atlas creation happens on worker threads; the first frames include one-time setup).
      no consumer   no block at all: not one "live share" line in the log (the layer only probes).
      idle block    the block exists but nobody heartbeats: the producer never starts, no "live share" line.
    Skips (exit 0 with a message) when amtrucks / eurotrucks2 / obs64 / Beat Saber is running.
#>
param([switch]$InstalledLayer, [string]$Only = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\Release'
$e2e = Join-Path $bin 'layer_e2e.exe'
$cons = Join-Path $bin 'live_share_test.exe'
foreach ($f in $e2e, $cons) { if (-not (Test-Path $f)) { throw "build first: $f" } }
$busy = Get-Process amtrucks, eurotrucks2, obs64, 'Beat Saber' -ErrorAction SilentlyContinue
if ($busy) {
    Write-Host ("run_live: skipped -- {0} is running (GPU tests would disturb it)" -f (($busy | ForEach-Object Name) -join ', ')) -ForegroundColor Yellow
    exit 0
}
if (-not $InstalledLayer) {
    $env:DISABLE_XR_APILAYER_JRDEVO_eye_capture = '1'
    $lp = Join-Path $root 'build\e2e_layer'
    New-Item -ItemType Directory -Force $lp | Out-Null
    $m = Get-Content (Join-Path $bin 'XR_APILAYER_JRDEVO_eye_capture.json') -Raw | ConvertFrom-Json
    $m.api_layer.library_path = (Join-Path $bin 'eye_capture.dll')
    $m | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $lp 'XR_APILAYER_JRDEVO_eye_capture.json') -Encoding ascii
    $env:XR_API_LAYER_PATH = $lp
    $env:XR_ENABLE_API_LAYERS = 'XR_APILAYER_JRDEVO_eye_capture'
    Write-Host "testing $bin\eye_capture.dll (registered copy disabled for these processes)" -ForegroundColor Yellow
}
# The OpenXR mock runtime implements only the subset these tests use: a foreign implicit API layer that the loader
# inserts into the chain (e.g. DienerTech VectorXR on this PC) can break it. Disable that layer for these processes
# (the real runtime is not touched; these run as a child process so the setting does not leak to the parent shell).
$env:DISABLE_XR_APILAYER_DIENERTECH_VECTORXR = '1'
# Every layer copy reads %APPDATA%\VR EyeCapture\config.ini first (config.cpp ConfigPath): on a PC with the installed
# app that is the user's own config (codec, size, layout), so the checks below could read it instead of the pinned
# defaults. An empty APPDATA for these processes -> the config next to the DLL (or the defaults). Real APPDATA untouched
# (child process). The per-case XR_EYE_CAPTURE_CONFIG still pins everything the checks depend on.
$appDataTest = Join-Path $root 'build\live_appdata'
New-Item -ItemType Directory -Force $appDataTest | Out-Null
$env:APPDATA = $appDataTest
# armed without a tray, nothing published to a running tray, no recording
$env:XR_EYE_CAPTURE_REQUIRE_TRAY = '0'
$env:XR_EYE_CAPTURE_TRAY_IPC = '0'
# everything the checks depend on is pinned (the code defaults) so the personal config.ini cannot change a result; the per-case
# keys come after it and win
$baseCfg = 'projection=flat;live_share=1;vr_indicator=1;indicator_live=1;swap_eyes=0;gamma=auto;max_width=0;max_fps=0;codec=hevc;audio_enabled=1;record_overlays=1;output_fov_scale=0.9;head_follow_deg=12;roll_lock=0;edge_guard=1'
$env:XR_EYE_CAPTURE_CONFIG = $baseCfg
$results = @()
$caseNo = 0

function ReadLog($out) {
    $f = Join-Path $out 'eye_capture.log'
    if (-not (Test-Path $f)) { return '' }
    $fs = [System.IO.File]::Open($f, 'Open', 'Read', 'ReadWrite')
    $t = (New-Object System.IO.StreamReader($fs, [Text.Encoding]::UTF8)).ReadToEnd()
    $fs.Dispose()
    return $t
}

# Starts layer_e2e in the background (stdout to a file), returns the process. $record > 0: autorecord that many seconds
# (an MP4 is expected: E2E_OUTPUT); $overlays: layer_e2e --overlays.
function StartGame($api, $layout, $seconds, $out, $shm, $jitter, $overlays = $false, $record = 0) {
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    New-Item -ItemType Directory -Force $out | Out-Null
    $env:XR_EYE_CAPTURE_TEST_LIVE_SHM = $shm
    $gargs = @('--api', $api, '--layout', $layout, '--seconds', "$seconds", '--record', "$record", '--out-dir', $out)
    if ($record -le 0) { $gargs += '--no-output' }
    if ($jitter) { $gargs += '--jitter' }
    if ($overlays) { $gargs += '--overlays' }
    $p = Start-Process -FilePath $e2e -ArgumentList $gargs -NoNewWindow -PassThru `
        -RedirectStandardOutput (Join-Path $out 'e2e_stdout.txt') -RedirectStandardError (Join-Path $out 'e2e_stderr.txt')
    $null = $p.Handle   # keep the process handle: without it ExitCode stays empty after the exit
    return $p
}

function WaitGame($p, $out) {
    if (-not $p.WaitForExit(60000)) { $p.Kill(); return $false }
    $o = Get-Content (Join-Path $out 'e2e_stdout.txt') -Raw -ErrorAction SilentlyContinue
    return ($p.ExitCode -eq 0) -and ([string]$o -match 'E2E_LOOP_OK|E2E_OUTPUT=')
}

# ---- MP4 pixels (record + live case): the mock's eyes (-+0.032, 1.6, 0), identity, fov +-0.8 rad, 1920x1080 per eye ----
$ffCand = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Filter ffmpeg.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
$ffmpegExe = if ($ffCand) { $ffCand.FullName } else { (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source }
function OvPx($x, $y, $z, $eye) {   # projection-space point -> pixel (x in the SBS frame, y)
    $T = [math]::Tan(0.8); $ex = if ($eye -eq 0) { -0.032 } else { 0.032 }
    $u = (($x - $ex) / (-$z) + $T) / (2 * $T); $v = ($T - ($y - 1.6) / (-$z)) / (2 * $T)
    return @([int][math]::Round($u * 1920 + 1920 * $eye), [int][math]::Round($v * 1080))
}
function BoxAvg($px, $cx, $cy, $dx, $dy) {
    $s = @(0.0, 0.0, 0.0); $n = 0
    for ($y = $cy - $dy; $y -le $cy + $dy; $y++) { for ($x = $cx - $dx; $x -le $cx + $dx; $x++) {
        $o = ($y * 3840 + $x) * 3; for ($k = 0; $k -lt 3; $k++) { $s[$k] += $px[$o + $k] }; $n++ } }
    return @([math]::Round($s[0] / $n), [math]::Round($s[1] / $n), [math]::Round($s[2] / $n))
}
function ColDiff($a, $b) { $m = 0; for ($k = 0; $k -lt 3; $k++) { $d = [math]::Abs($a[$k] - $b[$k]); if ($d -gt $m) { $m = $d } }; return $m }
# The overlay layers in the recorded frame at 3 s (the same points as live_share_test --expect overlays / run_e2e.ps1)
# and the indicator region (never recorded). Returns $true when all match.
function Mp4Overlays($file) {
    $raw = Join-Path ([System.IO.Path]::GetTempPath()) ("eyecap_live_{0}.rgb" -f $PID)
    & $ffmpegExe -v error -y -ss 3.0 -i $file -frames:v 1 -vf 'scale=in_color_matrix=bt709:in_range=limited:out_range=full,format=rgb24' -f rawvideo $raw
    $px = [System.IO.File]::ReadAllBytes($raw); Remove-Item $raw -ErrorAction SilentlyContinue
    $ok = $true
    foreach ($eye in 0, 1) {
        $band = if ($eye -eq 0) { @(200, 40, 40) } else { @(40, 40, 200) }
        foreach ($pt in @(@('A quad', 0.0, 1.45, -2.0, @(0, 255, 0), 20, 15), @('C cylinder', 0.5, 2.5, -1.5, @(255, 128, 0), 10, 6),
                          @('D back face (band)', 0.7, 1.41, -1.5, $band, 10, 6))) {
            $p = OvPx $pt[1] $pt[2] $pt[3] $eye; $a = BoxAvg $px $p[0] $p[1] $pt[5] $pt[6]
            $okP = (ColDiff $a $pt[4]) -le 12
            Write-Host ("  mp4 eye {0} {1}: {2} want {3} ({4})" -f $eye, $pt[0], ($a -join ','), ($pt[4] -join ','), $(if ($okP) { 'ok' } else { 'FAIL' })) -ForegroundColor $(if ($okP) { 'Green' } else { 'Red' })
            $ok = $ok -and $okP
        }
        $worst = 0
        for ($y = 620; $y -lt 670; $y += 2) { for ($x = 1176 + 1920 * $eye; $x -lt 1256 + 1920 * $eye; $x += 2) {
            $o = ($y * 3840 + $x) * 3; for ($k = 0; $k -lt 3; $k++) { $d = [math]::Abs($px[$o + $k] - $band[$k]); if ($d -gt $worst) { $worst = $d } } } }
        $okI = $worst -le 12
        Write-Host ("  mp4 eye {0} indicator region: worst {1} -> {2}" -f $eye, $worst, $(if ($okI) { 'ok: not recorded' } else { 'FAIL' })) -ForegroundColor $(if ($okI) { 'Green' } else { 'Red' })
        $ok = $ok -and $okI
    }
    return $ok
}

# ---- consumer cases ----
$rawCfg = 'symmetric_fov=1;head_smoothing=0.6'   # would re-project: the consumer's raw request must win
$smoothCfg = 'symmetric_fov=1;head_smoothing=0.6;head_follow_deg=12;roll_lock=0;output_fov_scale=0.9;edge_guard=1'
$ovCfg = 'symmetric_fov=1;head_smoothing=0.6;output_fov_scale=1;record_overlays=1'   # fov scale 1: the mock's pixel positions
$cases = @(
    @{ name = 'd3d11_two';         api = 'd3d11'; layout = 'two';   cfg = "$rawCfg;record_overlays=1"; overlays = $true; liveDot = 'log'
       cargs = @('--raw') },
    @{ name = 'd3d11_wide';        api = 'd3d11'; layout = 'wide';  cfg = "$rawCfg;indicator_live=0"; liveDot = 'off'; cargs = @('--raw') },
    @{ name = 'd3d12_two';         api = 'd3d12'; layout = 'two';   cfg = $rawCfg; cargs = @('--raw') },
    @{ name = 'd3d12_array';       api = 'd3d12'; layout = 'array'; cfg = $rawCfg; cargs = @('--raw') },
    @{ name = 'd3d12_two_abandon'; api = 'd3d12'; layout = 'two';   cfg = $rawCfg; cargs = @('--raw', '--abandon'); abandon = $true },
    @{ name = 'd3d11_two_smooth';  api = 'd3d11'; layout = 'two';   cfg = $smoothCfg; jitter = $true; seconds = 20; reproj = 'smooth'
       cargs = @('--eye', '1408x1440', '--expect', 'smooth', '--switch-raw') },
    @{ name = 'd3d12_two_sym';     api = 'd3d12'; layout = 'two';   cfg = 'symmetric_fov=1;head_smoothing=0'; reproj = 'sym'
       cargs = @('--expect', 'sym') },
    @{ name = 'd3d11_two_cfgoff';  api = 'd3d11'; layout = 'two';   cfg = 'symmetric_fov=0;head_smoothing=0;record_overlays=1'; cfgOff = $true; liveDot = 'on'
       cargs = @('--expect', 'raw') },
    @{ name = 'd3d11_two_half_raw'; api = 'd3d11'; layout = 'two';  cfg = $rawCfg; half = $true; cargs = @('--raw', '--half') },
    @{ name = 'd3d12_two_half_sym'; api = 'd3d12'; layout = 'two';  cfg = 'symmetric_fov=1;head_smoothing=0'; reproj = 'sym'; half = $true
       cargs = @('--half', '--expect', 'sym') },
    @{ name = 'd3d12_two_ov_half';  api = 'd3d12'; layout = 'two';  cfg = $ovCfg; overlays = $true; reproj = 'smooth'; half = $true
       ovRestart = 'yes'; seconds = 18; cargs = @('--half', '--expect', 'overlays', '--flags', '15') },
    @{ name = 'd3d11_two_rec_live'; api = 'd3d11'; layout = 'two';  cfg = $ovCfg; overlays = $true; reproj = 'smooth'; record = 8; ovRestart = 'any'
       seconds = 14; cargs = @('--expect', 'overlays', '--flags', '11') }
)
foreach ($c in $cases) {
    $caseNo++
    $name = $c.name
    if ($Only -and $name -notmatch $Only) { continue }
    $out = Join-Path $root "build\live_out\$name"
    $shm = "Local\OpenXR-EyeCapture-Live-Test-$PID-$caseNo"
    Write-Host "`n=== $name ($($c.api) / $($c.layout); config $($c.cfg); consumer $($c.cargs -join ' '))" -ForegroundColor Cyan
    $env:XR_EYE_CAPTURE_CONFIG = "$baseCfg;$($c.cfg)"
    $secs = if ($c.seconds) { $c.seconds } else { 14 }
    $rec = if ($c.record) { $c.record } else { 0 }
    $g = StartGame $c.api $c.layout $secs $out $shm $c.jitter ([bool]$c.overlays) $rec
    $cargs = @('--shm', $shm, '--timeout', '10') + $c.cargs
    $co = & $cons @cargs 2>&1 | Out-String
    $cExit = $LASTEXITCODE
    $co.Trim().Split("`n") | ForEach-Object { Write-Host "  $_" }
    $consOk = ($cExit -eq 0) -and ($co -match 'LIVE_OK')
    $gameOk = WaitGame $g $out
    $log = ReadLog $out
    $found = [regex]::Match($log, 'live share: consumer found[^\r\n]*')
    $ready = [regex]::Match($log, 'live share: ring ready[^\r\n]*')
    $stops = [regex]::Matches($log, 'live share: stopped \(([^)]*)\) after ([\d.]+) s -- frames shared (\d+), dropped busy (\d+)[^\r\n]*?(\d+) share calls, max ([\d.]+) ms, (\d+) over [^\r\n]*')
    $stop = if ($stops.Count) { $stops[$stops.Count - 1] } else { $null }
    $destroyIdx = $log.IndexOf('destroyed after')
    $stopBeforeEnd = $stop -and ($destroyIdx -lt 0 -or $stop.Index -lt $destroyIdx)
    $why = if ($stop) { $stop.Groups[1].Value } else { '' }
    $logOk = $found.Success -and $ready.Success -and $stop -and ($why -like 'consumer gone*') -and $stopBeforeEnd
    foreach ($l in $found, $ready) { if ($l.Success) { Write-Host ('  log: ' + $l.Value) } }
    foreach ($s in $stops) { Write-Host ('  log: ' + $s.Value) }
    # no game-side stall: every stop line reports share calls and none over 4 ms
    $stalls = 0
    foreach ($s in $stops) { if ([int]$s.Groups[7].Value -ne 0) { $stalls++ } }
    $stallOk = ($stops.Count -gt 0) -and ($stalls -eq 0) -and ([int]$stop.Groups[5].Value -gt 0)
    if (-not $stallOk) { Write-Host '  FAIL: game-thread share calls over 4 ms, or the last stop line without share calls' -ForegroundColor Red }
    $eo = [string](Get-Content (Join-Path $out 'e2e_stdout.txt') -Raw -ErrorAction SilentlyContinue)
    $ef = [regex]::Match($eo, 'E2E_ENDFRAME max ([\d.]+) ms \(frame (\d+)\), over 4 ms on (\d+) of (\d+) frames \((\d+) after the first second\)')
    if ($ef.Success) { Write-Host ('  game: ' + $ef.Value) }
    # the in-headset indicator (D3D11): the mock counts our quad layer; live dot on = shown while the viewer receives frames
    # 'on': no other quad layer in the game, so the mock's count is our dot: on some frames (while the viewer receives
    # frames), not all; 'off': indicator_live = 0, never; 'log': the game has its own quads (--overlays), only the log line
    $qd = [regex]::Match($eo, 'quad layers: valid on (\d+) frames, invalid (\d+)')
    $dotOk = $true
    if ($c.liveDot) {
        $nq = if ($qd.Success) { [int]$qd.Groups[1].Value } else { 0 }
        $total = if ($ef.Success) { [int]$ef.Groups[4].Value } else { 0 }
        $liveLine = $log -match 'indicator: quad layer appended AFTER the capture on frame \d+ \(\d+ game layers \+ 1, state live\)'
        $dotOk = if ($c.liveDot -eq 'off') { ($nq -eq 0) -and -not $liveLine }
                 elseif ($c.liveDot -eq 'on') { $liveLine -and ($nq -gt 0) -and ($nq -lt $total) }
                 else { $liveLine }
        Write-Host ("  indicator (live dot {0}): quad layer on {1} of {2} frames, 'state live' line {3} -> {4}" -f $c.liveDot, $nq, $total, $liveLine, $(if ($dotOk) { 'ok' } else { 'FAIL' })) -ForegroundColor $(if ($dotOk) { 'Green' } else { 'Red' })
    }
    # what the layer says it shares
    $modeOk = $true
    if ($c.reproj) {
        # the last episode's (an overlay restart leaves an earlier one with few frames)
        $sms = [regex]::Matches($log, 'live share: re-projection summary \(head smoothing (on|off), symmetric fov (on|off)\): (\d+) frames[^\r\n]*')
        $summary = if ($sms.Count) { $sms[$sms.Count - 1] } else { [regex]::Match('', 'x') }
        if ($summary.Success) { Write-Host ('  log: ' + $summary.Value) }
        $wantSmooth = if ($c.reproj -eq 'smooth') { 'on' } else { 'off' }
        $modeOk = ($found.Value -match 're-projected like the recording') -and ($ready.Value -match 're-projected') -and
                  $summary.Success -and ($summary.Groups[1].Value -eq $wantSmooth) -and ($summary.Groups[2].Value -eq 'on') -and
                  ([int]$summary.Groups[3].Value -gt 100)
        if ($c.half) { $modeOk = $modeOk -and ($found.Value -match 'half size') -and ($ready.Value -match 'half size') }
        if ($c.overlays) {
            $ovRing = [regex]::Matches($log, 'live share: ring ready[^\r\n]*\+ overlays[^\r\n]*')
            $ovSum = [regex]::Match($log, 'live share: overlay summary: ([1-9]\d*) frames with overlays composited[^\r\n]*atlas setup failed 0 [^\r\n]*')
            $ovAtl = [regex]::Match($log, 'live share: overlays: R8G8B8A8 atlas[^\r\n]*')
            foreach ($l in @($ovAtl, $ovSum)) { if ($l.Success) { Write-Host ('  log: ' + $l.Value) } }
            $modeOk = $modeOk -and ($ovRing.Count -ge 1) -and $ovSum.Success -and $ovAtl.Success
            $restart = [regex]::Match($log, 'live share: stopped \(the game shows overlay layers')
            # 'yes': a live-only game, the first ring has no overlays; 'any': with a recording the live share may have
            # learnt about them before its first ring (the recording's collection) or not (it started first)
            if ($c.ovRestart -eq 'yes') { $modeOk = $modeOk -and $restart.Success -and ($ovRing.Count -ge 1) -and ($ovRing[0].Index -gt $restart.Index) }
            elseif ($c.ovRestart -ne 'any') { $modeOk = $modeOk -and (-not $restart.Success) }
            if ($restart.Success) { Write-Host ('  log: ' + $restart.Value + ' ...') }
        }
        if ($c.reproj -eq 'smooth' -and -not $c.overlays) {   # the consumer switched to raw eyes: stop for that, then a raw ring
            $sw = [regex]::Match($log, 'live share: stopped \(the viewer switched to raw eyes')
            $rawRing = [regex]::Match($log, 'live share: ring ready[^\r\n]*raw eyes[^\r\n]*')
            if ($rawRing.Success) { Write-Host ('  log: ' + $rawRing.Value) }
            $modeOk = $modeOk -and $sw.Success -and $rawRing.Success -and ($rawRing.Index -gt $sw.Index)
        }
    } elseif ($c.cfgOff) {
        $modeOk = ($found.Value -match 'raw eyes \(symmetric_fov 0, head_smoothing 0') -and ($ready.Value -match 'raw eyes')
    } else {
        $modeOk = ($found.Value -match 'raw eyes \(the viewer asked for them\)') -and ($ready.Value -match 'raw eyes') -and
                  ($found.Value -match 'no overlays \(raw eyes\)') -and ($log -notmatch 'live share: overlay')
        if ($c.half) { $modeOk = $modeOk -and ($ready.Value -match 'raw eyes, half size \(identity pass\)') }
        else { $modeOk = $modeOk -and ($ready.Value -notmatch 'identity pass') }
    }
    # record + live: the MP4 (verify_mp4.ps1 without the pattern pixels, which the overlays cover; then the overlay pixels),
    # recorder drops 0, both overlay summaries, no xrEndFrame over 4 ms after the first second
    $recOk = $true
    if ($c.record) {
        $mp = [regex]::Match($eo, 'E2E_OUTPUT=(.+)')
        $recOk = $mp.Success
        if ($recOk) {
            $file = $mp.Groups[1].Value.Trim()
            Write-Host "  mp4: $file"
            & (Join-Path $PSScriptRoot 'verify_mp4.ps1') -File $file -EyeW 1920 -EyeH 1080 -Seconds ($c.record - 0.3) -DurationTolerance 0.3 -ExpectAudio -SkipPixels
            $recOk = ($LASTEXITCODE -eq 0) -and (Mp4Overlays $file)
        }
        $sum = [regex]::Match($log, 'recording summary: OK[^\r\n]*frames written (\d+), dropped (\d+) \(ring full (\d+), gpu busy (\d+), encoder stall (\d+)\)[^\r\n]*')
        $rOv = [regex]::Match($log, 'INFO\s+overlay summary: ([1-9]\d*) frames with overlays composited[^\r\n]*atlas setup failed 0 [^\r\n]*')
        if ($sum.Success) { Write-Host ('  log: ' + $sum.Value.Substring(0, [math]::Min(260, $sum.Value.Length))) }
        if ($rOv.Success) { Write-Host ('  log: recorder ' + $rOv.Value.Substring($rOv.Value.IndexOf('overlay summary'))) }
        # dropped 0, or only a few "ring full" at the start: run_e2e.ps1's overlay recordings (no live share) drop 2-3 that
        # way too (the worker makes the overlay atlases / compositor while the first frames queue up); gpu busy / encoder
        # stall must be 0 and the ring-full ones at most 3
        $dropOk = $sum.Success -and ([int]$sum.Groups[4].Value -eq 0) -and ([int]$sum.Groups[5].Value -eq 0) -and
                  ([int]$sum.Groups[3].Value -le 3)
        if ($sum.Success -and [int]$sum.Groups[2].Value -gt 0) {
            Write-Host ("  note: recorder dropped {0} (ring full {1}) -- the start-up overlay atlas case, see run_e2e.ps1 overlays" -f $sum.Groups[2].Value, $sum.Groups[3].Value) -ForegroundColor Yellow
        }
        $efOk = $ef.Success -and ([int]$ef.Groups[5].Value -eq 0)   # no xrEndFrame over 4 ms after the first second
        Write-Host ("  record + live: mp4 {0}, recorder drops ok {1}, recorder overlay summary {2}, xrEndFrame stalls {3}" -f $recOk, $dropOk, $rOv.Success, $efOk) -ForegroundColor $(if ($recOk -and $dropOk -and $rOv.Success -and $efOk) { 'Green' } else { 'Red' })
        $recOk = $recOk -and $dropOk -and $rOv.Success -and $efOk
    }
    if (-not $modeOk) { Write-Host '  FAIL: the layer log does not show the expected live-view mode' -ForegroundColor Red }
    $logOk = $logOk -and $stallOk -and $modeOk -and $dotOk -and $recOk
    Write-Host ("  consumer {0}; game loop {1}; layer log: found {2}, ring {3}, stopped by the consumer leaving (before the session ended) {4}, no stall {5}, mode {6}" -f
        $(if ($consOk) { 'ok' } else { 'FAIL' }), $(if ($gameOk) { 'ok' } else { 'FAIL' }), $found.Success, $ready.Success,
        ($stop -and ($why -like 'consumer gone*') -and $stopBeforeEnd), $stallOk, $modeOk) -ForegroundColor $(if ($consOk -and $gameOk -and $logOk) { 'Green' } else { 'Red' })
    $results += [pscustomobject]@{ case = $name; consumer = $consOk; game = $gameOk; log = $logOk }
}
$env:XR_EYE_CAPTURE_CONFIG = $baseCfg

if (-not $Only) {
# ---- no consumer: no block -> the layer only probes, nothing in the log ----
$caseNo++
$out = Join-Path $root 'build\live_out\no_consumer'
Write-Host "`n=== d3d11 / two, no consumer (no block)" -ForegroundColor Cyan
$g = StartGame 'd3d11' 'two' 4 $out "Local\OpenXR-EyeCapture-Live-Test-$PID-$caseNo" $false
$gameOk = WaitGame $g $out
$log = ReadLog $out
$quiet = ($log.Length -gt 0) -and ($log -notmatch 'live share:') -and ($log -match 'capture AVAILABLE')
Write-Host ("  game loop {0}; log has no 'live share' line: {1}" -f $gameOk, $quiet) -ForegroundColor $(if ($gameOk -and $quiet) { 'Green' } else { 'Red' })
$results += [pscustomobject]@{ case = 'no consumer'; consumer = $true; game = $gameOk; log = $quiet }

# ---- idle block: exists, never heartbeated -> the producer stays idle ----
$caseNo++
$out = Join-Path $root 'build\live_out\idle_block'
$shm = "Local\OpenXR-EyeCapture-Live-Test-$PID-$caseNo"
Write-Host "`n=== d3d11 / two, block without heartbeat" -ForegroundColor Cyan
$g = StartGame 'd3d11' 'two' 5 $out $shm $false
$co = & $cons --shm $shm --idle 4.5 2>&1 | Out-String
$consOk = ($LASTEXITCODE -eq 0) -and ($co -match 'LIVE_IDLE_OK')
$co.Trim().Split("`n") | ForEach-Object { Write-Host "  $_" }
$gameOk = WaitGame $g $out
$log = ReadLog $out
$quiet = ($log.Length -gt 0) -and ($log -notmatch 'live share:')
Write-Host ("  consumer {0}; game loop {1}; log has no 'live share' line: {2}" -f $consOk, $gameOk, $quiet) -ForegroundColor $(if ($consOk -and $gameOk -and $quiet) { 'Green' } else { 'Red' })
$results += [pscustomobject]@{ case = 'idle block'; consumer = $consOk; game = $gameOk; log = $quiet }
}

Remove-Item Env:\XR_EYE_CAPTURE_TEST_LIVE_SHM -ErrorAction SilentlyContinue
Write-Host ''
$results | Format-Table -AutoSize
if ($results | Where-Object { -not ($_.consumer -and $_.game -and $_.log) }) { exit 1 }
exit 0
